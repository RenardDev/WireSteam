@{
	IncludeRules = @(
		'PSPlaceOpenBrace',
		'PSPlaceCloseBrace',
		'PSUseConsistentIndentation',
		'PSUseConsistentWhitespace',
		'PSAvoidTrailingWhitespace'
	)
	Rules = @{
		PSPlaceOpenBrace = @{
			Enable = $true
			OnSameLine = $true
			NewLineAfter = $true
			IgnoreOneLineBlock = $false
		}
		PSPlaceCloseBrace = @{
			Enable = $true
			NoEmptyLineBefore = $false
			IgnoreOneLineBlock = $false
			NewLineAfter = $false
		}
		PSUseConsistentIndentation = @{
			Enable = $true
			Kind = 'tab'
			PipelineIndentation = 'IncreaseIndentationForFirstPipeline'
		}
		PSUseConsistentWhitespace = @{
			Enable = $true
			CheckInnerBrace = $true
			CheckOpenBrace = $true
			CheckOpenParen = $true
			CheckOperator = $true
			CheckPipe = $true
			CheckPipeForRedundantWhitespace = $true
			CheckSeparator = $true
			CheckParameter = $true
			IgnoreAssignmentOperatorInsideHashTable = $false
		}
		PSAvoidTrailingWhitespace = @{
			Enable = $true
		}
	}
}
